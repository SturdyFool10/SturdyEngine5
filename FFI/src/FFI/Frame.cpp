/// C ABI implementation of the per-frame render parameter setters.
///
/// Each entry point resolves the caller's scope-bound `SturdyFrame` back to the
/// `Engine::RenderFrameParameters` the engine is about to consume, validates its arguments, and
/// mutates that object in place. Validation is not defensive padding: a foreign caller has no
/// compiler stopping it from passing a NaN field of view or an enum value that does not exist, and
/// those would otherwise reach the renderer as undefined behavior rather than as an error.

#include <Foundation/Foundation.hpp>

#include <cmath>
#include <initializer_list>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::HandleKind;
    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_handle;
    using SFT::Ffi::set_error;
    using SFT::f32;

    /// Resolves `frame` to the render parameters it refers to.
    ///
    /// @param frame Handle supplied to `request_render_frame`.
    /// @param out_parameters Receives the borrowed parameters on success.
    ///
    /// @return `STURDY_OK`, or the handle failure `resolve_handle` reported.
    /// @note This function does not throw exceptions.
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

    /// Reports whether every supplied value is finite.
    ///
    /// NaN and infinity are the failure mode that matters most here: they propagate silently
    /// through matrix math and surface as a blank or corrupted frame far from the call that
    /// introduced them, so they are rejected at the boundary instead.
    ///
    /// @return Returns `true` when every value is finite; otherwise returns `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool all_finite(std::initializer_list<f32> values) noexcept {
        for (const f32 value : values) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        return true;
    }

    /// Translates an ABI render feature to the engine's own enumeration.
    ///
    /// @param feature Value received from the caller.
    /// @param out_feature Receives the translated value.
    ///
    /// @return Returns `true` when `feature` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_feature(SturdyRenderFeature feature,
                                         SFT::Engine::RenderFeature *out_feature) noexcept {
        switch (feature) {
        case STURDY_RENDER_FEATURE_SCENE:
            *out_feature = SFT::Engine::RenderFeature::Scene;
            return true;
        case STURDY_RENDER_FEATURE_SHADOWS:
            *out_feature = SFT::Engine::RenderFeature::Shadows;
            return true;
        case STURDY_RENDER_FEATURE_AMBIENT_OCCLUSION:
            *out_feature = SFT::Engine::RenderFeature::AmbientOcclusion;
            return true;
        case STURDY_RENDER_FEATURE_ANTI_ALIASING:
            *out_feature = SFT::Engine::RenderFeature::AntiAliasing;
            return true;
        case STURDY_RENDER_FEATURE_BLOOM:
            *out_feature = SFT::Engine::RenderFeature::Bloom;
            return true;
        case STURDY_RENDER_FEATURE_TONE_MAPPING:
            *out_feature = SFT::Engine::RenderFeature::ToneMapping;
            return true;
        case STURDY_RENDER_FEATURE_DEBUG_OVERLAY:
            *out_feature = SFT::Engine::RenderFeature::DebugOverlay;
            return true;
        case STURDY_RENDER_FEATURE_RESTIR_GI:
            *out_feature = SFT::Engine::RenderFeature::RestirGi;
            return true;
        case STURDY_RENDER_FEATURE_MOTION_BLUR:
            *out_feature = SFT::Engine::RenderFeature::MotionBlur;
            return true;
        case STURDY_RENDER_FEATURE_FORCE_U32:
        default:
            return false;
        }
    }

    /// Translates an ABI tone-mapping operator to the engine's own enumeration.
    ///
    /// @param operation Value received from the caller.
    /// @param out_operation Receives the translated value.
    ///
    /// @return Returns `true` when `operation` is a value this build recognizes; otherwise `false`.
    /// @note This function does not throw exceptions.
    [[nodiscard]] bool translate_tone_mapping(SturdyToneMapping operation,
                                              SFT::Engine::ToneMappingOperator *out_operation) noexcept {
        switch (operation) {
        case STURDY_TONE_MAPPING_NONE:
            *out_operation = SFT::Engine::ToneMappingOperator::None;
            return true;
        case STURDY_TONE_MAPPING_REINHARD:
            *out_operation = SFT::Engine::ToneMappingOperator::Reinhard;
            return true;
        case STURDY_TONE_MAPPING_EXPONENTIAL:
            *out_operation = SFT::Engine::ToneMappingOperator::Exponential;
            return true;
        case STURDY_TONE_MAPPING_AGX:
            *out_operation = SFT::Engine::ToneMappingOperator::Agx;
            return true;
        case STURDY_TONE_MAPPING_HERMITE_SPLINE:
            *out_operation = SFT::Engine::ToneMappingOperator::HermiteSpline;
            return true;
        case STURDY_TONE_MAPPING_PSYCHO_V:
            *out_operation = SFT::Engine::ToneMappingOperator::PsychoV;
            return true;
        case STURDY_TONE_MAPPING_FORCE_U32:
        default:
            return false;
        }
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_frame_set_camera_position(SturdyFrame frame,
                                                              float x,
                                                              float y,
                                                              float z) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!all_finite({x, y, z})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "camera position must be finite");
        }
        parameters->camera.set_position(glm::vec3{x, y, z});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_camera_look_at(SturdyFrame frame,
                                                         float x,
                                                         float y,
                                                         float z) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!all_finite({x, y, z})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "camera target must be finite");
        }

        const glm::vec3 target{x, y, z};
        // look_at() derives an orientation from the position-to-target vector, which is
        // degenerate when they coincide. Reject that here rather than letting it produce a
        // silently unusable (NaN) orientation.
        if (target == parameters->camera.position()) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "camera target must differ from the camera position");
        }
        parameters->camera.look_at(target);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_camera_perspective(SturdyFrame frame,
                                                                 float vertical_fov_degrees,
                                                                 float near_clip,
                                                                 float far_clip) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!all_finite({vertical_fov_degrees, near_clip, far_clip})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "perspective parameters must be finite");
        }
        if (vertical_fov_degrees <= 0.0f || vertical_fov_degrees >= 180.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "vertical field of view must be within (0, 180) degrees");
        }
        if (near_clip <= 0.0f || far_clip <= near_clip) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "clip planes must satisfy 0 < near_clip < far_clip");
        }
        parameters->camera.set_perspective(vertical_fov_degrees, near_clip, far_clip);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_camera_viewport(SturdyFrame frame,
                                                              uint32_t width,
                                                              uint32_t height) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (width == 0 || height == 0) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "viewport dimensions must be nonzero");
        }
        parameters->camera.set_viewport_size(width, height);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_ambient_light(SturdyFrame frame,
                                                            float red,
                                                            float green,
                                                            float blue,
                                                            float exposure) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!all_finite({red, green, blue, exposure})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "ambient light values must be finite");
        }
        if (red < 0.0f || green < 0.0f || blue < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "ambient radiance must not be negative");
        }
        if (exposure <= 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "exposure must be greater than zero");
        }
        parameters->lighting.ambient_radiance = glm::vec3{red, green, blue};
        parameters->lighting.exposure = exposure;
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_background_color(SturdyFrame frame,
                                                               float red,
                                                               float green,
                                                               float blue,
                                                               float alpha) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!all_finite({red, green, blue, alpha})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "background color must be finite");
        }
        parameters->render_graph.set_background_color(glm::vec4{red, green, blue, alpha});
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_feature_enabled(SturdyFrame frame,
                                                              SturdyRenderFeature feature,
                                                              SturdyBool enabled) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Engine::RenderFeature engine_feature{};
        if (!translate_feature(feature, &engine_feature)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized render feature");
        }
        parameters->render_graph.set_enabled(engine_feature, enabled != STURDY_FALSE);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_tone_mapping(SturdyFrame frame,
                                                           SturdyToneMapping operation,
                                                           float exposure,
                                                           float white_point,
                                                           float saturation) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }

        SFT::Engine::ToneMappingOperator engine_operation{};
        if (!translate_tone_mapping(operation, &engine_operation)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized tone mapping operator");
        }
        if (!all_finite({exposure, white_point, saturation})) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "tone mapping values must be finite");
        }
        if (exposure <= 0.0f || white_point <= 0.0f || saturation < 0.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "exposure and white point must be positive, saturation non-negative");
        }
        parameters->render_graph.set_tone_mapping(engine_operation, exposure, white_point, saturation);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_resolution_scale(SturdyFrame frame, float scale) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        if (!all_finite({scale}) || scale <= 0.0f || scale > 2.0f) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "resolution scale must be within (0, 2]");
        }
        parameters->render_graph.set_resolution_scale(scale);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_frame_set_debug_label(SturdyFrame frame, const char *label) {
    return guarded([&]() -> SturdyResult {
        SFT::Engine::RenderFrameParameters *parameters = nullptr;
        const SturdyResult resolved = resolve_frame(frame, &parameters);
        if (resolved != STURDY_OK) {
            return resolved;
        }
        parameters->debug_label = label == nullptr ? SFT::UString{} : SFT::UString{label};
        return STURDY_OK;
    });
}

} // extern "C"
