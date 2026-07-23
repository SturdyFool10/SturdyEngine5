#include "RenderGraph.hpp"

#include <algorithm>
#include <cmath>

namespace SFT::Engine {

    namespace {
        [[nodiscard]] bool finite(glm::vec4 value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z) && std::isfinite(value.w);
        }
    } // namespace

    RenderGraph::RenderGraph(RenderGraphDescription description) noexcept
        : description_(std::move(description)) {}

    RenderGraph RenderGraph::standard() noexcept { return {}; }

    RenderGraph RenderGraph::overlay_only() noexcept {
        RenderGraph graph;
        graph.description_.scene.enabled = false;
        graph.description_.shadows.enabled = false;
        graph.description_.bloom.enabled = false;
        graph.description_.debug_overlay.enabled = true;
        return graph;
    }

    const RenderGraphDescription &RenderGraph::description() const noexcept { return description_; }
    RenderGraphDescription &RenderGraph::description() noexcept { return description_; }
    const SceneRenderSettings &RenderGraph::scene() const noexcept { return description_.scene; }
    SceneRenderSettings &RenderGraph::scene() noexcept { return description_.scene; }
    const ShadowSettings &RenderGraph::shadows() const noexcept { return description_.shadows; }
    ShadowSettings &RenderGraph::shadows() noexcept { return description_.shadows; }
    const BloomSettings &RenderGraph::bloom() const noexcept { return description_.bloom; }
    BloomSettings &RenderGraph::bloom() noexcept { return description_.bloom; }
    const ToneMappingSettings &RenderGraph::tone_mapping() const noexcept { return description_.tone_mapping; }
    ToneMappingSettings &RenderGraph::tone_mapping() noexcept { return description_.tone_mapping; }
    const DebugOverlayRenderSettings &RenderGraph::debug_overlay() const noexcept { return description_.debug_overlay; }
    DebugOverlayRenderSettings &RenderGraph::debug_overlay() noexcept { return description_.debug_overlay; }
    RenderGraphExecutionMode RenderGraph::execution_mode() const noexcept { return description_.execution_mode; }

    bool RenderGraph::enabled(RenderFeature feature) const noexcept {
        switch (feature) {
            case RenderFeature::Scene:
                return description_.scene.enabled;
            case RenderFeature::Shadows:
                return description_.shadows.enabled;
            case RenderFeature::Bloom:
                return description_.bloom.enabled;
            case RenderFeature::ToneMapping:
                return description_.tone_mapping.enabled;
            case RenderFeature::DebugOverlay:
                return description_.debug_overlay.enabled;
        }
        return false;
    }

    RenderGraph &RenderGraph::set_enabled(RenderFeature feature, bool enabled_value) noexcept {
        switch (feature) {
            case RenderFeature::Scene:
                description_.scene.enabled = enabled_value;
                break;
            case RenderFeature::Shadows:
                description_.shadows.enabled = enabled_value;
                break;
            case RenderFeature::Bloom:
                description_.bloom.enabled = enabled_value;
                break;
            case RenderFeature::ToneMapping:
                description_.tone_mapping.enabled = enabled_value;
                break;
            case RenderFeature::DebugOverlay:
                description_.debug_overlay.enabled = enabled_value;
                break;
        }
        return *this;
    }

    RenderGraph &RenderGraph::enable(RenderFeature feature) noexcept { return set_enabled(feature, true); }
    RenderGraph &RenderGraph::disable(RenderFeature feature) noexcept { return set_enabled(feature, false); }

    RenderGraph &RenderGraph::set_execution_mode(RenderGraphExecutionMode mode) noexcept {
        description_.execution_mode = mode;
        return *this;
    }

    RenderGraph &RenderGraph::set_resolution_scale(f32 scale) noexcept {
        description_.resolution_scale = scale;
        return *this;
    }

    RenderGraph &RenderGraph::set_background_color(glm::vec4 color) noexcept {
        description_.scene.background_color = color;
        return *this;
    }

    RenderGraph &RenderGraph::inherit_camera_background() noexcept {
        description_.scene.background_color.reset();
        return *this;
    }

    RenderGraph &RenderGraph::set_tone_mapping(ToneMappingOperator operation,
                                               f32 exposure,
                                               f32 white_point,
                                               f32 saturation) noexcept {
        description_.tone_mapping.enabled = operation != ToneMappingOperator::None;
        description_.tone_mapping.operation = operation;
        description_.tone_mapping.exposure = exposure;
        description_.tone_mapping.white_point = white_point;
        description_.tone_mapping.saturation = saturation;
        return *this;
    }

    RenderGraphResult RenderGraph::validate() const noexcept {
        if (!std::isfinite(description_.resolution_scale) ||
            description_.resolution_scale < 0.1f || description_.resolution_scale > 2.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidResolutionScale,
                .message = UString{"Render graph resolution_scale must be finite and in [0.1, 2.0]."_ustr},
            });
        }
        if (description_.scene.background_color && !finite(*description_.scene.background_color)) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidBackgroundColor,
                .message = UString{"Render graph background color must contain only finite values."_ustr},
            });
        }
        if (!std::isfinite(description_.scene.background_intensity) ||
            description_.scene.background_intensity < 0.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidBackgroundColor,
                .message = UString{"Render graph background intensity must be finite and non-negative."_ustr},
            });
        }
        const ShadowSettings &shadows = description_.shadows;
        if (shadows.atlas_size < 512 || shadows.atlas_size > 16384 || shadows.atlas_size % 8 != 0 ||
            shadows.cascade_count < 1 || shadows.cascade_count > 4 ||
            !std::isfinite(shadows.max_distance) || shadows.max_distance <= 0.0f ||
            !std::isfinite(shadows.cascade_split_lambda) || shadows.cascade_split_lambda < 0.0f || shadows.cascade_split_lambda > 1.0f ||
            !std::isfinite(shadows.cascade_blend) || shadows.cascade_blend < 0.0f || shadows.cascade_blend > 0.5f ||
            !std::isfinite(shadows.depth_bias) || shadows.depth_bias < 0.0f ||
            !std::isfinite(shadows.slope_bias) || shadows.slope_bias < 0.0f ||
            !std::isfinite(shadows.normal_bias) || shadows.normal_bias < 0.0f || shadows.normal_bias > 4.0f ||
            shadows.max_shadowed_spot_lights > 8 || shadows.max_shadowed_point_lights > 4) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidShadowSettings,
                .message = UString{"Shadow settings are outside their supported atlas, cascade, bias, or local-light ranges."_ustr},
            });
        }
        const BloomSettings &bloom = description_.bloom;
        if (!std::isfinite(bloom.threshold) || bloom.threshold < 0.0f ||
            !std::isfinite(bloom.soft_knee) || bloom.soft_knee < 0.0f || bloom.soft_knee > 1.0f ||
            !std::isfinite(bloom.intensity) || bloom.intensity < 0.0f ||
            !std::isfinite(bloom.scatter) || bloom.scatter < 0.0f || bloom.scatter > 1.0f ||
            bloom.max_levels < 1 || bloom.max_levels > 10) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidBloomSettings,
                .message = UString{"Render graph bloom requires a non-negative threshold/intensity, soft_knee and scatter in [0, 1], and max_levels in [1, 10]."_ustr},
            });
        }
        const ToneMappingSettings &tone = description_.tone_mapping;
        if (!std::isfinite(tone.exposure) || tone.exposure < 0.0f ||
            !std::isfinite(tone.white_point) || tone.white_point <= 0.0f ||
            !std::isfinite(tone.saturation) || tone.saturation < 0.0f || tone.saturation > 4.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph tonemapping requires finite non-negative exposure, positive white point, and saturation in [0, 4]."_ustr},
            });
        }
        if (!std::isfinite(tone.hdr_paper_white_nits) || tone.hdr_paper_white_nits <= 0.0f ||
            !std::isfinite(tone.hdr_peak_nits) || tone.hdr_peak_nits < tone.hdr_paper_white_nits) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph HDR output requires a positive paper-white and a peak nits value at or above it."_ustr},
            });
        }
        if (!std::isfinite(tone.hermite_spline.toe_strength) || tone.hermite_spline.toe_strength < 0.0f || tone.hermite_spline.toe_strength > 1.0f ||
            !std::isfinite(tone.hermite_spline.toe_length) || tone.hermite_spline.toe_length < 0.0f || tone.hermite_spline.toe_length > 1.0f ||
            !std::isfinite(tone.hermite_spline.shoulder_strength) || tone.hermite_spline.shoulder_strength < 0.0f ||
            !std::isfinite(tone.hermite_spline.shoulder_length) || tone.hermite_spline.shoulder_length < 0.0f || tone.hermite_spline.shoulder_length > 1.0f ||
            !std::isfinite(tone.hermite_spline.shoulder_angle) || tone.hermite_spline.shoulder_angle < 0.0f || tone.hermite_spline.shoulder_angle > 1.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph Hermite spline settings must be finite; toe/shoulder length and shoulder angle must lie in [0, 1], and shoulder strength must be non-negative."_ustr},
            });
        }
        if (!std::isfinite(tone.psycho_v.highlights) || tone.psycho_v.highlights <= 0.0f ||
            !std::isfinite(tone.psycho_v.shadows) || tone.psycho_v.shadows <= 0.0f ||
            !std::isfinite(tone.psycho_v.contrast) || tone.psycho_v.contrast <= 0.0f ||
            !std::isfinite(tone.psycho_v.purity_scale) || tone.psycho_v.purity_scale < 0.0f ||
            !std::isfinite(tone.psycho_v.gamut_compression) || tone.psycho_v.gamut_compression < 0.0f || tone.psycho_v.gamut_compression > 1.0f ||
            !std::isfinite(tone.psycho_v.compression) || tone.psycho_v.compression < 0.0f ||
            !finite(glm::vec4{tone.psycho_v.adapted_gray_bt709, 1.0f}) ||
            !finite(glm::vec4{tone.psycho_v.background_gray_bt709, 1.0f}) ||
            tone.psycho_v.adapted_gray_bt709.x <= 0.0f || tone.psycho_v.adapted_gray_bt709.y <= 0.0f ||
            tone.psycho_v.adapted_gray_bt709.z <= 0.0f || tone.psycho_v.background_gray_bt709.x <= 0.0f ||
            tone.psycho_v.background_gray_bt709.y <= 0.0f || tone.psycho_v.background_gray_bt709.z <= 0.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidToneMappingSettings,
                .message = UString{"Render graph PsychoV settings must be finite, with positive highlights/shadows/contrast, non-negative purity/compression, gamut_compression in [0, 1], and positive adapted/background gray points."_ustr},
            });
        }

        return {};
    }

    RenderGraph RenderGraph::normalized() const noexcept {
        RenderGraph result = *this;
        RenderGraphDescription &desc = result.description_;
        desc.resolution_scale = std::isfinite(desc.resolution_scale)
                                    ? std::clamp(desc.resolution_scale, 0.1f, 2.0f)
                                    : 1.0f;
        if (desc.scene.background_color && !finite(*desc.scene.background_color)) {
            desc.scene.background_color.reset();
        }
        desc.scene.background_intensity = std::isfinite(desc.scene.background_intensity)
                                              ? std::max(desc.scene.background_intensity, 0.0f)
                                              : 1.0f;
        ShadowSettings &shadows = desc.shadows;
        shadows.atlas_size = std::clamp(shadows.atlas_size, 512u, 16384u);
        shadows.atlas_size -= shadows.atlas_size % 8u;
        shadows.cascade_count = std::clamp(shadows.cascade_count, 1u, 4u);
        shadows.max_distance = std::isfinite(shadows.max_distance) && shadows.max_distance > 0.0f
                                   ? shadows.max_distance : 250.0f;
        shadows.cascade_split_lambda = std::isfinite(shadows.cascade_split_lambda)
                                           ? std::clamp(shadows.cascade_split_lambda, 0.0f, 1.0f) : 0.65f;
        shadows.cascade_blend = std::isfinite(shadows.cascade_blend)
                                    ? std::clamp(shadows.cascade_blend, 0.0f, 0.5f) : 0.10f;
        shadows.depth_bias = std::isfinite(shadows.depth_bias) ? std::max(shadows.depth_bias, 0.0f) : 0.75f;
        shadows.slope_bias = std::isfinite(shadows.slope_bias) ? std::max(shadows.slope_bias, 0.0f) : 1.0f;
        shadows.normal_bias = std::isfinite(shadows.normal_bias)
                                  ? std::clamp(shadows.normal_bias, 0.0f, 4.0f)
                                  : 0.75f;
        shadows.max_shadowed_spot_lights = std::min(shadows.max_shadowed_spot_lights, 8u);
        shadows.max_shadowed_point_lights = std::min(shadows.max_shadowed_point_lights, 4u);
        desc.bloom.threshold = std::isfinite(desc.bloom.threshold) ? std::max(desc.bloom.threshold, 0.0f) : 1.0f;
        desc.bloom.soft_knee = std::isfinite(desc.bloom.soft_knee) ? std::clamp(desc.bloom.soft_knee, 0.0f, 1.0f) : 0.5f;
        desc.bloom.intensity = std::isfinite(desc.bloom.intensity) ? std::max(desc.bloom.intensity, 0.0f) : 0.08f;
        desc.bloom.scatter = std::isfinite(desc.bloom.scatter) ? std::clamp(desc.bloom.scatter, 0.0f, 1.0f) : 0.7f;
        desc.bloom.max_levels = std::clamp(desc.bloom.max_levels, 1u, 10u);
        desc.tone_mapping.exposure = std::isfinite(desc.tone_mapping.exposure)
                                         ? std::max(desc.tone_mapping.exposure, 0.0f)
                                         : 1.0f;
        desc.tone_mapping.white_point = std::isfinite(desc.tone_mapping.white_point) && desc.tone_mapping.white_point > 0.0f
                                            ? desc.tone_mapping.white_point
                                            : 1.0f;
        desc.tone_mapping.saturation = std::isfinite(desc.tone_mapping.saturation)
                                           ? std::clamp(desc.tone_mapping.saturation, 0.0f, 4.0f)
                                           : 1.0f;
        desc.tone_mapping.hdr_paper_white_nits = std::isfinite(desc.tone_mapping.hdr_paper_white_nits) && desc.tone_mapping.hdr_paper_white_nits > 0.0f
                                                     ? desc.tone_mapping.hdr_paper_white_nits
                                                     : 203.0f;
        desc.tone_mapping.hdr_peak_nits = std::isfinite(desc.tone_mapping.hdr_peak_nits)
                                              ? std::max(desc.tone_mapping.hdr_peak_nits, desc.tone_mapping.hdr_paper_white_nits)
                                              : 1000.0f;
        HermiteSplineSettings &hermite = desc.tone_mapping.hermite_spline;
        hermite.toe_strength = std::isfinite(hermite.toe_strength) ? std::clamp(hermite.toe_strength, 0.0f, 1.0f) : 0.5f;
        hermite.toe_length = std::isfinite(hermite.toe_length) ? std::clamp(hermite.toe_length, 0.0f, 1.0f) : 0.5f;
        hermite.shoulder_strength = std::isfinite(hermite.shoulder_strength) ? std::max(hermite.shoulder_strength, 0.0f) : 2.0f;
        hermite.shoulder_length = std::isfinite(hermite.shoulder_length) ? std::clamp(hermite.shoulder_length, 0.0f, 1.0f) : 0.5f;
        hermite.shoulder_angle = std::isfinite(hermite.shoulder_angle) ? std::clamp(hermite.shoulder_angle, 0.0f, 1.0f) : 1.0f;
        PsychoVSettings &psycho_v = desc.tone_mapping.psycho_v;
        psycho_v.highlights = std::isfinite(psycho_v.highlights) && psycho_v.highlights > 0.0f ? psycho_v.highlights : 1.0f;
        psycho_v.shadows = std::isfinite(psycho_v.shadows) && psycho_v.shadows > 0.0f ? psycho_v.shadows : 1.0f;
        psycho_v.contrast = std::isfinite(psycho_v.contrast) && psycho_v.contrast > 0.0f ? psycho_v.contrast : 1.0f;
        psycho_v.purity_scale = std::isfinite(psycho_v.purity_scale) ? std::max(psycho_v.purity_scale, 0.0f) : 1.0f;
        psycho_v.gamut_compression = std::isfinite(psycho_v.gamut_compression) ? std::clamp(psycho_v.gamut_compression, 0.0f, 1.0f) : 1.0f;
        psycho_v.compression = std::isfinite(psycho_v.compression) ? std::max(psycho_v.compression, 0.0f) : 0.0f;
        if (!finite(glm::vec4{psycho_v.adapted_gray_bt709, 1.0f}) ||
            psycho_v.adapted_gray_bt709.x <= 0.0f || psycho_v.adapted_gray_bt709.y <= 0.0f || psycho_v.adapted_gray_bt709.z <= 0.0f) {
            psycho_v.adapted_gray_bt709 = glm::vec3{0.18f};
        }
        if (!finite(glm::vec4{psycho_v.background_gray_bt709, 1.0f}) ||
            psycho_v.background_gray_bt709.x <= 0.0f || psycho_v.background_gray_bt709.y <= 0.0f || psycho_v.background_gray_bt709.z <= 0.0f) {
            psycho_v.background_gray_bt709 = glm::vec3{0.18f};
        }

        return result;
    }

} // namespace SFT::Engine
