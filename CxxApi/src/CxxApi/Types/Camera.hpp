#pragma once

// Plain-data camera description. bindgen-safe (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

#include <CxxApi/Types/Common.hpp>

namespace SFT::CxxApi {

    enum class CameraProjection : std::uint32_t {
        Perspective = 0,
        Orthographic = 1,
        /// Uses `CameraDesc::custom_projection` as the projection matrix.
        Custom = 2,
    };

    /// Mirrors `Engine::CameraContainment`.
    enum class Containment : std::uint32_t {
        Outside = 0,
        Intersecting = 1,
        Inside = 2,
    };

    /// Every settable field of `Engine::Camera`. Member defaults equal a default-constructed
    /// `Engine::Camera` (checked by `CxxApiCameraTest`).
    struct CameraDesc {
        CameraProjection projection = CameraProjection::Perspective;
        float position[3] = {0.0f, 0.0f, 0.0f};
        float euler_degrees[3] = {0.0f, 0.0f, 0.0f};
        /// When true, `orientation` (xyzw quaternion) is used instead of `euler_degrees`.
        bool has_orientation = false;
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float vertical_fov_degrees = 60.0f;
        float orthographic_size = 10.0f;
        /// Column-major; used when `projection == Custom`.
        float custom_projection[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                       0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        float aspect_ratio = 16.0f / 9.0f;
        float near_clip = 0.05f;
        float far_clip = 1000.0f;
        float lens_shift[2] = {0.0f, 0.0f};
        float jitter_ndc[2] = {0.0f, 0.0f};
        bool reverse_z = false;
        /// When true, `focal_length_mm` is applied after the projection (and so overrides the FOV).
        bool has_focal_length = false;
        float focal_length_mm = 20.7846f;
        float sensor_size_mm[2] = {36.0f, 24.0f};
        float aperture_f_stop = 2.8f;
        float shutter_seconds = 1.0f / 60.0f;
        float iso = 100.0f;
        float focus_distance = 10.0f;
        std::uint32_t aperture_blades = 7;
        float exposure_compensation_ev = 0.0f;
        std::uint32_t culling_mask = 0xFFFFFFFFu;
        std::int32_t priority = 0;
        bool active = true;
        float clear_color[4] = {0.01f, 0.015f, 0.025f, 1.0f};
        float render_scale = 1.0f;
        /// `x, y, width, height`, normalized.
        float normalized_viewport[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        /// Drop temporal history (previous view-projection) before this frame — a camera cut.
        bool reset_history = false;
    };

} // namespace SFT::CxxApi
