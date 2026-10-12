// The CxxApi body-rig surface: POD defaults equal the engine's, settings round-trip, and the functions drive the rig.

#include <CxxApi/BodyRig.hpp>

#include <cmath>
#include <cstdio>
#include <type_traits>

using namespace SFT;
using namespace SFT::CxxApi;

namespace {
    int failures = 0;
    void check(bool ok, const char *what) {
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", what);
            ++failures;
        }
    }
    bool near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) <= eps; }
    static_assert(std::is_standard_layout_v<BodyRigDesc> && std::is_trivially_copyable_v<BodyRigDesc>);
    static_assert(std::is_standard_layout_v<BodyRigPose> && std::is_trivially_copyable_v<BodyRigPose>);
} // namespace

int main() {
    // The POD's member defaults and the defaults function both equal the engine's.
    const Engine::BodyCameraRigSettings engine_defaults;
    const BodyRigDesc member_defaults;
    const BodyRigDesc function_defaults = body_rig_desc_defaults();
    check(near(member_defaults.position_follow_hz, engine_defaults.position_follow_hz) && near(member_defaults.rotation_follow_hz, engine_defaults.rotation_follow_hz) &&
              near(member_defaults.damping_ratio, engine_defaults.damping_ratio) && near(member_defaults.stride_length, engine_defaults.stride_length) &&
              near(member_defaults.gait_reference_speed, engine_defaults.gait_reference_speed) && near(member_defaults.bob_amplitude, engine_defaults.bob_amplitude) &&
              near(member_defaults.sway_amplitude, engine_defaults.sway_amplitude) && near(member_defaults.roll_amplitude_degrees, engine_defaults.roll_amplitude_degrees) &&
              near(member_defaults.pitch_amplitude_degrees, engine_defaults.pitch_amplitude_degrees) && near(member_defaults.gait_blend_rate, engine_defaults.gait_blend_rate) &&
              near(member_defaults.handheld_rotation_degrees, engine_defaults.handheld_rotation_degrees) && near(member_defaults.handheld_position, engine_defaults.handheld_position) &&
              near(member_defaults.handheld_frequency_hz, engine_defaults.handheld_frequency_hz) && member_defaults.seed == engine_defaults.seed,
          "BodyRigDesc member defaults equal the engine's");
    check(near(function_defaults.bob_amplitude, member_defaults.bob_amplitude) && function_defaults.seed == member_defaults.seed, "body_rig_desc_defaults agrees");

    // Settings round-trip through the rig.
    BodyRigDesc custom = member_defaults;
    custom.stride_length = 2.25f;
    custom.seed = 99;
    custom.handheld_position = 0.0f;
    custom.handheld_rotation_degrees = 0.0f;
    custom.bob_amplitude = 0.0f;
    custom.sway_amplitude = 0.0f;
    custom.roll_amplitude_degrees = 0.0f;
    custom.pitch_amplitude_degrees = 0.0f;
    auto rig = body_rig_new(custom);
    check(near(body_rig_settings(*rig).stride_length, 2.25f) && body_rig_settings(*rig).seed == 99, "settings round-trip");

    // Reset, follow, impulse and the matrix.
    body_rig_reset(*rig, {1, 2, 3}, {0, 0, 0, 1});
    BodyRigPose pose = body_rig_pose(*rig);
    check(near(pose.position[0], 1.0f) && near(pose.position[2], 3.0f) && near(pose.orientation[3], 1.0f), "reset snaps to the pose");
    for (int i = 0; i < 240; ++i) body_rig_update(*rig, 1.0 / 60.0, {5, 2, 3}, {0, 0, 0, 1}, 0.0f);
    pose = body_rig_pose(*rig);
    check(near(pose.position[0], 5.0f, 1e-2f), "the camera follows the aim");
    body_rig_add_translation_impulse(*rig, {0, 4, 0});
    body_rig_update(*rig, 1.0 / 60.0, {5, 2, 3}, {0, 0, 0, 1}, 0.0f);
    check(body_rig_pose(*rig).position[1] > 2.0f, "an impulse kicks the camera");
    for (int i = 0; i < 120; ++i) body_rig_update(*rig, 1.0 / 60.0, {5, 2, 3}, {0, 0, 0, 1}, 0.0f);
    const std::array<float, 16> matrix = body_rig_matrix(*rig);
    check(near(matrix[12], 5.0f, 1e-2f) && near(matrix[13], 2.0f, 1e-2f) && near(matrix[15], 1.0f), "the matrix carries the position (column-major)");
    check(near(body_rig_gait_amount(*rig), 0.0f), "no speed, no gait");

    if (failures == 0) std::printf("CxxApiBodyRigTest: all checks passed.\n");
    return failures == 0 ? 0 : 1;
}
