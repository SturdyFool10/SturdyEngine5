#pragma once

#include <Foundation/Foundation.hpp>

#include <array>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace SFT::Renderer {

    /// GPU-resident surfel record. `position_radius.w == 0` marks an unclaimed ring-buffer slot.
    struct alignas(16) SurfelGpuData {
        glm::vec4 position_radius{0.0f};
        glm::vec4 normal_cascade{0.0f};
        glm::vec4 irradiance_confidence{0.0f};
    };
    static_assert(sizeof(SurfelGpuData) == 48);

    /// Fixed-capacity spatial hash-grid cell; entries beyond `kSurfelGridCellCapacity` are dropped
    /// rather than chained, trading a small amount of coverage for a single-pass, prefix-sum-free build.
    inline constexpr u32 kSurfelGridCellCapacity = 8;

    struct alignas(16) SurfelGridCellGpuData {
        u32 count = 0;
        std::array<u32, kSurfelGridCellCapacity> indices{};
    };
    static_assert(sizeof(SurfelGridCellGpuData) == 48);

    struct alignas(16) SurfelGiFrameConstants {
        glm::mat4 inverse_view_projection{1.0f};
        glm::vec4 camera_position_max_surfels{};
        glm::vec4 cascade_distances{};
        glm::vec4 grid_params{};
        glm::vec4 spawn_params{};
        glm::vec4 temporal_intensity{};
        glm::vec4 extent_frame_index{};
        glm::vec4 sun_direction_angular_radius{};
        glm::vec4 sun_radiance{};
        glm::vec4 sky_color_intensity{};
    };
    static_assert(sizeof(SurfelGiFrameConstants) == 208);

} // namespace SFT::Renderer
