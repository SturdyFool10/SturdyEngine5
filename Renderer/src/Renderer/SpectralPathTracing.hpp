#pragma once

#include <Foundation/src/Foundation.hpp>

namespace SFT::Renderer {


    enum class SpectralRenderMode : u8 {
        RasterDeferred,
        ShadowOnly,
        ReflectionOnly,
        AmbientOcclusionOnly,
        ShadowAndTransmission,
        FullPathTracing,
    };

    struct SpectralPathTracingSettings {
        SpectralRenderMode mode = SpectralRenderMode::RasterDeferred;
        u32 samples_per_pixel = 1;
        u32 max_bounces = 8;
        u32 russian_roulette_start_bounce = 3;
        u32 photon_count = 262144;
        f32 caustic_gather_radius = 0.075f;
        f32 wavelength_min_nm = 380.0f;
        f32 wavelength_max_nm = 780.0f;
    };

    struct SpectralIntegratorPolicy {
        bool uses_ray_queries = false;
        bool raster_primary_visibility = true;
        bool raster_shadow_atlas = true;
        bool raster_deferred_lighting = true;
        bool writes_canonical_gbuffer = false;
        bool traces_transmission = false;

    };

    /// Performs the spectral integrator policy operation for `Renderer` using the supplied arguments.
    ///
    /// @param mode Mode controlling how the operation is performed.
    ///
    /// @return Returns the value produced by the operation.
    /// @note This function does not throw exceptions.
    [[nodiscard]] constexpr SpectralIntegratorPolicy spectral_integrator_policy(SpectralRenderMode mode) noexcept {
        switch (mode) {
            case SpectralRenderMode::RasterDeferred:
                return {};
            case SpectralRenderMode::ShadowOnly:
                return {.uses_ray_queries = true};
            case SpectralRenderMode::ReflectionOnly:
                return {.uses_ray_queries = true};
            case SpectralRenderMode::AmbientOcclusionOnly:
                return {.uses_ray_queries = true};
            case SpectralRenderMode::ShadowAndTransmission:
                return {
                    .uses_ray_queries = true,
                    .traces_transmission = true,
                };
            case SpectralRenderMode::FullPathTracing:
                return {
                    .uses_ray_queries = true,
                    .raster_primary_visibility = false,
                    .raster_shadow_atlas = false,
                    .raster_deferred_lighting = false,
                    .writes_canonical_gbuffer = true,
                    .traces_transmission = true,
                };
        }
        return {};
    }

} // namespace SFT::Renderer
