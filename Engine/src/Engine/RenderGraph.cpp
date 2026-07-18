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
        graph.description_.lighting.enabled = false;
        graph.description_.debug_overlay.enabled = true;
        return graph;
    }

    const RenderGraphDescription &RenderGraph::description() const noexcept { return description_; }
    RenderGraphDescription &RenderGraph::description() noexcept { return description_; }
    const SceneRenderSettings &RenderGraph::scene() const noexcept { return description_.scene; }
    SceneRenderSettings &RenderGraph::scene() noexcept { return description_.scene; }
    const LightingRenderSettings &RenderGraph::lighting() const noexcept { return description_.lighting; }
    LightingRenderSettings &RenderGraph::lighting() noexcept { return description_.lighting; }
    const ToneMappingSettings &RenderGraph::tone_mapping() const noexcept { return description_.tone_mapping; }
    ToneMappingSettings &RenderGraph::tone_mapping() noexcept { return description_.tone_mapping; }
    const DebugOverlayRenderSettings &RenderGraph::debug_overlay() const noexcept { return description_.debug_overlay; }
    DebugOverlayRenderSettings &RenderGraph::debug_overlay() noexcept { return description_.debug_overlay; }

    bool RenderGraph::enabled(RenderFeature feature) const noexcept {
        switch (feature) {
            case RenderFeature::Scene:
                return description_.scene.enabled;
            case RenderFeature::DeferredLighting:
                return description_.lighting.enabled;
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
            case RenderFeature::DeferredLighting:
                description_.lighting.enabled = enabled_value;
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
        if (!std::isfinite(description_.lighting.background_intensity) ||
            description_.lighting.background_intensity < 0.0f) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidLightingSettings,
                .message = UString{"Render graph background intensity must be finite and non-negative."_ustr},
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
        if (description_.scene.enabled && !description_.lighting.enabled) {
            return std::unexpected(RenderGraphError{
                .code = RenderGraphErrorCode::InvalidFeatureCombination,
                .message = UString{"The deferred scene feature requires deferred lighting; disable both for an overlay-only graph."_ustr},
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
        desc.lighting.background_intensity = std::isfinite(desc.lighting.background_intensity)
                                                 ? std::max(desc.lighting.background_intensity, 0.0f)
                                                 : 1.0f;
        desc.tone_mapping.exposure = std::isfinite(desc.tone_mapping.exposure)
                                         ? std::max(desc.tone_mapping.exposure, 0.0f)
                                         : 1.0f;
        desc.tone_mapping.white_point = std::isfinite(desc.tone_mapping.white_point) && desc.tone_mapping.white_point > 0.0f
                                            ? desc.tone_mapping.white_point
                                            : 1.0f;
        desc.tone_mapping.saturation = std::isfinite(desc.tone_mapping.saturation)
                                           ? std::clamp(desc.tone_mapping.saturation, 0.0f, 4.0f)
                                           : 1.0f;
        if (!desc.lighting.enabled) {
            desc.scene.enabled = false;
        }
        return result;
    }

} // namespace SFT::Engine
