#pragma once

// Plain-data settings for the body-camera rig. bindgen-safe (see plans/cxx-api.md).

#include <cstddef>
#include <cstdint>

namespace SFT::CxxApi {

    /// Mirrors `Engine::BodyCameraRigSettings`. Member defaults equal the engine's (checked by `CxxApiBodyRigTest`).
    struct BodyRigDesc {
        float position_follow_hz = 9.0f;
        float rotation_follow_hz = 5.0f;
        float damping_ratio = 1.0f;
        float stride_length = 1.5f;
        float gait_reference_speed = 1.6f;
        float bob_amplitude = 0.035f;
        float sway_amplitude = 0.02f;
        float roll_amplitude_degrees = 0.7f;
        float pitch_amplitude_degrees = 0.45f;
        float gait_blend_rate = 6.0f;
        float handheld_rotation_degrees = 0.18f;
        float handheld_position = 0.004f;
        float handheld_frequency_hz = 0.8f;
        std::uint32_t seed = 1;
    };

    /// A pose: position and an xyzw orientation quaternion.
    struct BodyRigPose {
        float position[3] = {0.0f, 0.0f, 0.0f};
        float orientation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    };

} // namespace SFT::CxxApi
