#pragma once

#include <Foundation/Foundation.hpp>

#include <array>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace SFT::Renderer {

    /// GPU-resident ReSTIR GI reservoir: one indirect-bounce path sample per GI-grid cell, reused across
    /// frames (temporal) and neighboring cells (spatial) via weighted reservoir sampling. Mirrors
    /// `restir_gi_common.slang`: `radiance_rgb9e5` is the kept sample's outgoing radiance in a shared-
    /// exponent RGB9E5 word; `weight_m_half` packs the unbiased contribution weight `W` (low 16 bits,
    /// half float) and the clamped sample count `M` (high 16 bits, half float; M == 0 means empty). The
    /// sample's position/normal and the running weight sum are deliberately not stored — no later pass
    /// reads them — which is what shrinks the reservoir from 64 to 8 bytes.
    struct ReservoirGpuData {
        u32 radiance_rgb9e5 = 0;
        u32 weight_m_half = 0;
    };
    static_assert(sizeof(ReservoirGpuData) == 8);

    /// Per-pixel ray-guiding cache consumed and updated by `restir_gi_initial_sample.slang`: a
    /// temporally-accumulated estimate of the dominant incoming-light direction seen from this pixel's
    /// shading point, used to bias next frame's primary bounce-direction sampling via resampled
    /// importance sampling (RIS) rather than sampling the cosine hemisphere blindly. Reprojected via
    /// motion vectors and ping-ponged the same way the reservoir buffers are. `direction` is a unit
    /// vector (world space); `strength` is the mean luminance of the traced radiance that produced it,
    /// used to scale how strongly the guide lobe biases sampling (see `restir_gi_initial_sample.slang`).
    ///
    /// `normal_distance` records the GI-grid cell's own surface for the temporal reuse pass to validate
    /// reprojected history against: xyz = world normal, w = distance from the camera (0 = no surface).
    struct alignas(16) GuideGpuData {
        glm::vec4 direction_strength{0.0f};
        glm::vec4 normal_distance{0.0f};
    };
    static_assert(sizeof(GuideGpuData) == 32);

    /// Fixed-capacity packed light list uploaded alongside `RestirGiFrameConstants` so ray-traced hit
    /// points can perform colored multi-light NEE (`sturdy_light_sampling.slang`) instead of only ever
    /// sampling the sun.
    inline constexpr u32 kRestirGiMaxLights = 8;

    struct alignas(16) RestirGiPackedLight {
        glm::vec4 position_range{0.0f};
        glm::vec4 radiance_source_radius{0.0f};
    };
    static_assert(sizeof(RestirGiPackedLight) == 32);

    /// GI-grid downscale relative to the G-buffer for a `RestirGiSettings::quality` value: Low/Medium
    /// trace one ray per 2x2 pixel block (a quarter of the rays), High traces every pixel.
    [[nodiscard]] inline u32 restir_gi_grid_scale(u32 quality) noexcept {
        return quality >= 2u ? 1u : 2u;
    }

    /// Upper bound quality places on spatial-reuse taps (settings may ask for fewer).
    [[nodiscard]] inline u32 restir_gi_max_spatial_taps(u32 quality) noexcept {
        return quality == 0u ? 1u : 2u;
    }

    /// Upper bound quality places on SVGF a-trous iterations (settings may ask for fewer).
    [[nodiscard]] inline u32 restir_gi_max_atrous_iterations(u32 quality) noexcept {
        return quality == 0u ? 2u : 3u;
    }

    struct alignas(16) RestirGiFrameConstants {
        glm::mat4 inverse_view_projection{1.0f};
        glm::mat4 previous_view_projection{1.0f};
        glm::vec4 camera_position_frame_index{};
        /// xy = GI grid extent, z = grid downscale relative to the G-buffer, w = max ray distance.
        glm::vec4 extent_max_ray_distance{};
        glm::vec4 sun_direction_angular_radius{};
        glm::vec4 sun_radiance{};
        /// x = temporal history cap (M clamp), y = spatial reuse sample count, z = spatial reuse radius
        /// in pixels, w = GI intensity multiplier.
        glm::vec4 temporal_spatial_params{};
        /// x = active light count, y = multi-bounce feedback strength, z = previous-frame buffers valid
        /// (0/1, false on the first frame or after a resolution change), w unused.
        glm::vec4 light_count_params{};
        std::array<RestirGiPackedLight, kRestirGiMaxLights> lights{};
    };
    static_assert(sizeof(RestirGiFrameConstants) == 64 * 2 + 16 * 6 + 32 * kRestirGiMaxLights);

} // namespace SFT::Renderer
