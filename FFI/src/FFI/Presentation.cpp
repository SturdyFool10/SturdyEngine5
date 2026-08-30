/// C ABI implementation of the per-surface presentation/HDR control surface.
///
/// These bind directly to `Engine::{set_presentation_settings, presentation_settings,
/// presentation_resolution, query_hdr_capabilities, update_hdr_content_light_level}`, one layer
/// above the raw `RhiDevice` swapchain calls — the same seam `Renderer::WindowSurfaceRecord`
/// already uses. A change here is queued exactly like a window request (see WindowTime.cpp):
/// applied the next time the surface renders, never synchronously, since a foreign caller might
/// invoke this from inside its own render callback while that surface's swapchain is in use.

#include <Foundation/Foundation.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>

#include <Engine/Engine.hpp>

#include <FFI/AbiSupport.hpp>

namespace {

    using SFT::Ffi::guarded;
    using SFT::Ffi::resolve_engine;
    using SFT::Ffi::set_error;

    [[nodiscard]] SFT::Core::RenderSurfaceHandle to_render_surface(SturdySurface surface) noexcept {
        return SFT::Core::RenderSurfaceHandle{
            static_cast<SFT::WindowManager::WindowId>(surface.id)};
    }

    [[nodiscard]] bool translate_vsync(SturdyVSync vsync, SFT::Core::VSyncMode *out_mode) noexcept {
        switch (vsync) {
        case STURDY_VSYNC_OFF:
            *out_mode = SFT::Core::VSyncMode::Off;
            return true;
        case STURDY_VSYNC_ON:
            *out_mode = SFT::Core::VSyncMode::On;
            return true;
        case STURDY_VSYNC_ADAPTIVE:
            *out_mode = SFT::Core::VSyncMode::Adaptive;
            return true;
        case STURDY_VSYNC_FORCE_U32:
        default:
            return false;
        }
    }

    /// Casts an ABI enum to its matching engine enum, rejecting values past the last one this
    /// build knows about. Sound only for the description enums declared in lockstep with their
    /// `RHI::`/`Core::` counterpart (see the "Presentation and HDR" section header in Sturdy.h),
    /// the same precedent as `SturdyFormat` and friends in RhiResources.cpp.
    template <typename Engine, typename Abi>
    [[nodiscard]] bool translate_ranged(Abi value, Engine max_value, Engine *out) noexcept {
        const auto raw = static_cast<std::underlying_type_t<Engine>>(value);
        if (raw > static_cast<std::underlying_type_t<Engine>>(max_value)) {
            return false;
        }
        *out = static_cast<Engine>(raw);
        return true;
    }

    [[nodiscard]] SturdyResult translate_rhi_error(const SFT::RHI::RhiError &error) noexcept {
        switch (error.code) {
        case SFT::RHI::RhiErrorCode::OutOfMemory:
            return set_error(STURDY_ERROR_OUT_OF_MEMORY, error.message);
        case SFT::RHI::RhiErrorCode::DeviceLost:
            return set_error(STURDY_ERROR_DEVICE_LOST, error.message);
        case SFT::RHI::RhiErrorCode::InvalidArgument:
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, error.message);
        case SFT::RHI::RhiErrorCode::Unsupported:
        case SFT::RHI::RhiErrorCode::NotReady:
            return set_error(STURDY_ERROR_NOT_AVAILABLE, error.message);
        case SFT::RHI::RhiErrorCode::OperationFailed:
        case SFT::RHI::RhiErrorCode::SurfaceLost:
        case SFT::RHI::RhiErrorCode::FullScreenExclusiveLost:
        default:
            return set_error(STURDY_ERROR_INTERNAL, error.message);
        }
    }

    [[nodiscard]] SturdyResult translate_graphics_backend_error(
        const SFT::Core::GraphicsBackendError &error) noexcept {
        switch (error.code) {
        case SFT::Core::GraphicsBackendErrorCode::OutOfMemory:
            return set_error(STURDY_ERROR_OUT_OF_MEMORY, error.message);
        case SFT::Core::GraphicsBackendErrorCode::DeviceLost:
            return set_error(STURDY_ERROR_DEVICE_LOST, error.message);
        case SFT::Core::GraphicsBackendErrorCode::Unsupported:
            return set_error(STURDY_ERROR_NOT_AVAILABLE, error.message);
        case SFT::Core::GraphicsBackendErrorCode::InitializationFailed:
        case SFT::Core::GraphicsBackendErrorCode::SurfaceLost:
        case SFT::Core::GraphicsBackendErrorCode::FullScreenExclusiveLost:
        case SFT::Core::GraphicsBackendErrorCode::OperationFailed:
        default:
            return set_error(STURDY_ERROR_INTERNAL, error.message);
        }
    }

    void copy_presentation_settings(const SFT::Core::PresentationSettings &settings,
                                     SturdyPresentationSettings *out) noexcept {
        *out = SturdyPresentationSettings{};
        out->struct_size = static_cast<uint32_t>(sizeof(SturdyPresentationSettings));
        out->vsync = static_cast<SturdyVSync>(settings.vsync);
        out->variable_refresh = static_cast<SturdyVariableRefreshMode>(settings.variable_refresh);
        out->latency = static_cast<SturdyLatencyMode>(settings.latency);
        out->preference = static_cast<SturdyPresentationPreference>(settings.preference);
        out->hdr_enabled = settings.hdr_enabled ? STURDY_TRUE : STURDY_FALSE;
        out->hdr_color_space = static_cast<SturdyHdrColorSpaceMode>(settings.hdr_color_space);
        out->transparent_composition = settings.transparent_composition ? STURDY_TRUE : STURDY_FALSE;
        out->swapchain_image_count = settings.swapchain_image_count;
        out->allow_present_from_compute = settings.allow_present_from_compute ? STURDY_TRUE : STURDY_FALSE;
    }

    void copy_presentation_resolution(const SFT::RHI::PresentationResolution &resolution,
                                       SturdyPresentationResolution *out) noexcept {
        *out = SturdyPresentationResolution{};
        out->struct_size = static_cast<uint32_t>(sizeof(SturdyPresentationResolution));
        out->strategy = static_cast<SturdyPresentStrategy>(resolution.strategy);
        out->effective_mode = static_cast<SturdyPresentMode>(resolution.effective_mode);
        out->degraded = resolution.degraded ? STURDY_TRUE : STURDY_FALSE;
        out->present_queue_is_compute = resolution.present_queue_is_compute ? STURDY_TRUE : STURDY_FALSE;
        out->effective_composite_alpha =
            static_cast<SturdyCompositeAlphaMode>(resolution.effective_composite_alpha);
        out->composite_alpha_degraded = resolution.composite_alpha_degraded ? STURDY_TRUE : STURDY_FALSE;
        out->via_composition_present = resolution.via_composition_present ? STURDY_TRUE : STURDY_FALSE;
        out->supports_completion_fence = resolution.supports_completion_fence ? STURDY_TRUE : STURDY_FALSE;
        out->full_screen_exclusive_active =
            resolution.full_screen_exclusive_active ? STURDY_TRUE : STURDY_FALSE;
        out->effective_format = static_cast<SturdyFormat>(resolution.effective_format);
        out->effective_color_space = static_cast<SturdyColorSpace>(resolution.effective_color_space);
    }

    void copy_chromaticity(const SFT::RHI::Chromaticity &value, SturdyChromaticity *out) noexcept {
        out->x = value.x;
        out->y = value.y;
    }

    void copy_display_metadata(const SFT::RHI::HdrDisplayMetadata &metadata,
                               SturdyHdrDisplayMetadata *out) noexcept {
        *out = SturdyHdrDisplayMetadata{};
        out->struct_size = static_cast<uint32_t>(sizeof(SturdyHdrDisplayMetadata));
        copy_chromaticity(metadata.red_primary, &out->red_primary);
        copy_chromaticity(metadata.green_primary, &out->green_primary);
        copy_chromaticity(metadata.blue_primary, &out->blue_primary);
        copy_chromaticity(metadata.white_point, &out->white_point);
        out->min_luminance_nits = metadata.min_luminance_nits;
        out->max_luminance_nits = metadata.max_luminance_nits;
        out->max_full_frame_luminance_nits = metadata.max_full_frame_luminance_nits;
        out->source = static_cast<SturdyHdrMetadataSource>(metadata.source);
        out->confidence = static_cast<SturdyHdrMetadataConfidence>(metadata.confidence);
    }

} // namespace

extern "C" {

SturdyResult STURDY_ABI_CALL sturdy_presentation_settings_init(SturdyPresentationSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        copy_presentation_settings(SFT::Core::PresentationSettings{}, settings);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_surface_set_presentation_settings(
    SturdyEngine engine,
    SturdySurface surface,
    const SturdyPresentationSettings *settings) {
    return guarded([&]() -> SturdyResult {
        if (settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "settings must not be null");
        }
        if (settings->struct_size != sizeof(SturdyPresentationSettings)) {
            return set_error(STURDY_ERROR_UNSUPPORTED_STRUCT_SIZE,
                             "SturdyPresentationSettings::struct_size does not match this build");
        }

        SFT::Core::PresentationSettings engine_settings{};
        if (!translate_vsync(settings->vsync, &engine_settings.vsync)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized vsync mode");
        }
        if (!translate_ranged(settings->variable_refresh, SFT::Core::VariableRefreshMode::Preferred,
                              &engine_settings.variable_refresh)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized variable refresh mode");
        }
        if (!translate_ranged(settings->latency, SFT::Core::LatencyMode::Ultra, &engine_settings.latency)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized latency mode");
        }
        if (!translate_ranged(settings->preference, SFT::Core::PresentationPreference::PowerEfficient,
                              &engine_settings.preference)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized presentation preference");
        }
        if (!translate_ranged(settings->hdr_color_space, SFT::Core::HdrColorSpaceMode::DolbyVision,
                              &engine_settings.hdr_color_space)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "unrecognized HDR color-space mode");
        }
        engine_settings.hdr_enabled = settings->hdr_enabled != STURDY_FALSE;
        engine_settings.transparent_composition = settings->transparent_composition != STURDY_FALSE;
        engine_settings.swapchain_image_count = settings->swapchain_image_count;
        engine_settings.allow_present_from_compute = settings->allow_present_from_compute != STURDY_FALSE;

        SFT::Engine::Engine *resolved_engine = nullptr;
        if (const SturdyResult resolved = resolve_engine(engine, &resolved_engine); resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Core::RendererResult applied =
            resolved_engine->set_presentation_settings(to_render_surface(surface), engine_settings);
        if (!applied.has_value()) {
            return translate_graphics_backend_error(applied.error());
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_surface_presentation_settings(
    SturdyEngine engine,
    SturdySurface surface,
    SturdyPresentationSettings *out_settings) {
    return guarded([&]() -> SturdyResult {
        if (out_settings == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::Engine *resolved_engine = nullptr;
        if (const SturdyResult resolved = resolve_engine(engine, &resolved_engine); resolved != STURDY_OK) {
            return resolved;
        }
        copy_presentation_settings(resolved_engine->presentation_settings(to_render_surface(surface)),
                                   out_settings);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_surface_presentation_resolution(
    SturdyEngine engine,
    SturdySurface surface,
    SturdyPresentationResolution *out_resolution) {
    return guarded([&]() -> SturdyResult {
        if (out_resolution == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "output pointer must not be null");
        }
        SFT::Engine::Engine *resolved_engine = nullptr;
        if (const SturdyResult resolved = resolve_engine(engine, &resolved_engine); resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::Core::RenderSurfaceHandle handle = to_render_surface(surface);
        // Engine::presentation_resolution() answers a default-constructed value for an unknown or
        // not-yet-rendered surface rather than failing; distinguish that from a real answer by
        // checking sturdy_window_find first, since a truly present-but-degraded-to-defaults
        // resolution is indistinguishable from "no swapchain yet" otherwise.
        if (resolved_engine->window_state().find(
                static_cast<SFT::WindowManager::WindowId>(surface.id)) == nullptr) {
            return set_error(STURDY_ERROR_NOT_AVAILABLE, "no managed window matches that surface");
        }
        copy_presentation_resolution(resolved_engine->presentation_resolution(handle), out_resolution);
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_surface_query_hdr_capabilities(
    SturdyEngine engine,
    SturdySurface surface,
    SturdyHdrCapabilities *out_capabilities,
    SturdyHdrPresentationMode *out_modes,
    uint32_t modes_capacity,
    uint32_t *out_mode_count) {
    return guarded([&]() -> SturdyResult {
        if (out_capabilities == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "out_capabilities must not be null");
        }
        if (modes_capacity > 0 && out_modes == nullptr) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT,
                             "out_modes must not be null when modes_capacity is nonzero");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        if (const SturdyResult resolved = resolve_engine(engine, &resolved_engine); resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::RHI::RhiExpected<SFT::RHI::SurfaceHdrCapabilityQuery> queried =
            resolved_engine->query_hdr_capabilities(to_render_surface(surface));
        if (!queried.has_value()) {
            return translate_rhi_error(queried.error());
        }

        const SFT::RHI::SurfaceHdrCapabilities &capabilities = queried->capabilities;
        *out_capabilities = SturdyHdrCapabilities{};
        out_capabilities->struct_size = static_cast<uint32_t>(sizeof(SturdyHdrCapabilities));
        out_capabilities->status =
            static_cast<SturdyPlatformQueryStatus>(queried->message.status);
        out_capabilities->hdr_supported = capabilities.hdr_supported ? STURDY_TRUE : STURDY_FALSE;
        out_capabilities->hdr_enabled_by_os = capabilities.hdr_enabled_by_os ? STURDY_TRUE : STURDY_FALSE;
        out_capabilities->hdr_metadata_output_supported =
            capabilities.hdr_metadata_output_supported ? STURDY_TRUE : STURDY_FALSE;
        out_capabilities->sdr_white_nits = capabilities.sdr_white_nits;
        out_capabilities->edr_headroom = capabilities.edr_headroom;
        out_capabilities->max_edr_headroom = capabilities.max_edr_headroom;
        if (capabilities.display_metadata.has_value()) {
            out_capabilities->has_display_metadata = STURDY_TRUE;
            copy_display_metadata(*capabilities.display_metadata, &out_capabilities->display_metadata);
        }

        if (out_mode_count != nullptr) {
            *out_mode_count = static_cast<uint32_t>(capabilities.supported_modes.size());
        }
        const size_t written = std::min<size_t>(modes_capacity, capabilities.supported_modes.size());
        for (size_t i = 0; i < written; ++i) {
            const SFT::RHI::HdrPresentationMode &mode = capabilities.supported_modes[i];
            out_modes[i] = SturdyHdrPresentationMode{
                static_cast<SturdyHdrTransferFunction>(mode.transfer),
                static_cast<SturdyHdrColorGamut>(mode.gamut),
                mode.requires_os_hdr_mode ? STURDY_TRUE : STURDY_FALSE,
            };
        }
        return STURDY_OK;
    });
}

SturdyResult STURDY_ABI_CALL sturdy_surface_update_hdr_content_light_level(
    SturdyEngine engine,
    SturdySurface surface,
    float max_content_light_level_nits,
    float max_frame_average_light_level_nits) {
    return guarded([&]() -> SturdyResult {
        if (!std::isfinite(max_content_light_level_nits) ||
            !std::isfinite(max_frame_average_light_level_nits)) {
            return set_error(STURDY_ERROR_INVALID_ARGUMENT, "light level values must be finite");
        }

        SFT::Engine::Engine *resolved_engine = nullptr;
        if (const SturdyResult resolved = resolve_engine(engine, &resolved_engine); resolved != STURDY_OK) {
            return resolved;
        }

        const SFT::RHI::HdrContentLightLevelUpdate update{
            .max_content_light_level_nits = max_content_light_level_nits,
            .max_frame_average_light_level_nits = max_frame_average_light_level_nits,
        };
        const SFT::RHI::RhiResult updated =
            resolved_engine->update_hdr_content_light_level(to_render_surface(surface), update);
        if (!updated.has_value()) {
            return translate_rhi_error(updated.error());
        }
        return STURDY_OK;
    });
}

} // extern "C"
