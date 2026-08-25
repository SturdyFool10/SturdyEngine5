#pragma once

#include <Foundation/Foundation.hpp>

#include <array>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace SFT::Renderer {

    /// GPU-resident ReSTIR GI reservoir: one indirect-bounce path sample per screen pixel, reused across
    /// frames (temporal) and neighboring pixels (spatial) via weighted reservoir sampling.
    /// `sample_position_pdf` holds the world-space secondary hit point (xyz) and the direction pdf the
    /// sample was originally drawn with (w). `sample_radiance_normal_x` holds outgoing radiance at that
    /// hit point (xyz) and the x component of its octahedral-encoded normal (w); `normal_y_valid` holds
    /// the encoded normal's y component (x) and a validity flag (y, 0 = empty reservoir). `weight_m`
    /// packs the reservoir's running weight sum (x), its resolved unbiased contribution weight `W` (y),
    /// and its clamped temporal sample count `M` (z).
    struct alignas(16) ReservoirGpuData {
        glm::vec4 sample_position_pdf{0.0f};
        glm::vec4 sample_radiance_normal_x{0.0f};
        glm::vec4 normal_y_valid{0.0f};
        glm::vec4 weight_m{0.0f};
    };
    static_assert(sizeof(ReservoirGpuData) == 64);

    /// Per-pixel ray-guiding cache consumed and updated by `restir_gi_initial_sample.slang`: a
    /// temporally-accumulated estimate of the dominant incoming-light direction seen from this pixel's
    /// shading point, used to bias next frame's primary bounce-direction sampling via resampled
    /// importance sampling (RIS) rather than sampling the cosine hemisphere blindly. Reprojected via
    /// motion vectors and ping-ponged the same way the reservoir buffers are. `direction` is a unit
    /// vector (world space); `strength` is the mean luminance of the traced radiance that produced it,
    /// used to scale how strongly the guide lobe biases sampling (see `restir_gi_initial_sample.slang`).
    struct alignas(16) GuideGpuData {
        glm::vec4 direction_strength{0.0f};
    };
    static_assert(sizeof(GuideGpuData) == 16);

    /// Fixed-capacity packed light list uploaded alongside `RestirGiFrameConstants` so ray-traced hit
    /// points can perform colored multi-light NEE (`sturdy_light_sampling.slang`) instead of only ever
    /// sampling the sun.
    inline constexpr u32 kRestirGiMaxLights = 8;

    struct alignas(16) RestirGiPackedLight {
        glm::vec4 position_range{0.0f};
        glm::vec4 radiance_source_radius{0.0f};
    };
    static_assert(sizeof(RestirGiPackedLight) == 32);

    struct alignas(16) RestirGiFrameConstants {
        glm::mat4 inverse_view_projection{1.0f};
        glm::mat4 previous_view_projection{1.0f};
        glm::vec4 camera_position_frame_index{};
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
