#include <CxxApi/BodyRig.hpp>

#include <CxxApi/Conversions.hpp>

namespace SFT::CxxApi {

    namespace {
        [[nodiscard]] Engine::BodyCameraRigSettings to_settings(const BodyRigDesc &d) noexcept {
            Engine::BodyCameraRigSettings s;
            s.position_follow_hz = d.position_follow_hz;
            s.rotation_follow_hz = d.rotation_follow_hz;
            s.damping_ratio = d.damping_ratio;
            s.stride_length = d.stride_length;
            s.gait_reference_speed = d.gait_reference_speed;
            s.bob_amplitude = d.bob_amplitude;
            s.sway_amplitude = d.sway_amplitude;
            s.roll_amplitude_degrees = d.roll_amplitude_degrees;
            s.pitch_amplitude_degrees = d.pitch_amplitude_degrees;
            s.gait_blend_rate = d.gait_blend_rate;
            s.handheld_rotation_degrees = d.handheld_rotation_degrees;
            s.handheld_position = d.handheld_position;
            s.handheld_frequency_hz = d.handheld_frequency_hz;
            s.seed = d.seed;
            return s;
        }

        [[nodiscard]] BodyRigDesc to_desc(const Engine::BodyCameraRigSettings &s) noexcept {
            BodyRigDesc d;
            d.position_follow_hz = s.position_follow_hz;
            d.rotation_follow_hz = s.rotation_follow_hz;
            d.damping_ratio = s.damping_ratio;
            d.stride_length = s.stride_length;
            d.gait_reference_speed = s.gait_reference_speed;
            d.bob_amplitude = s.bob_amplitude;
            d.sway_amplitude = s.sway_amplitude;
            d.roll_amplitude_degrees = s.roll_amplitude_degrees;
            d.pitch_amplitude_degrees = s.pitch_amplitude_degrees;
            d.gait_blend_rate = s.gait_blend_rate;
            d.handheld_rotation_degrees = s.handheld_rotation_degrees;
            d.handheld_position = s.handheld_position;
            d.handheld_frequency_hz = s.handheld_frequency_hz;
            d.seed = s.seed;
            return d;
        }
    } // namespace

    BodyRigDesc body_rig_desc_defaults() noexcept { return to_desc(Engine::BodyCameraRigSettings{}); }

    std::unique_ptr<Engine::BodyCameraRig> body_rig_new(const BodyRigDesc &desc) {
        return std::make_unique<Engine::BodyCameraRig>(to_settings(desc));
    }

    void body_rig_set_settings(Engine::BodyCameraRig &rig, const BodyRigDesc &desc) noexcept { rig.set_settings(to_settings(desc)); }

    BodyRigDesc body_rig_settings(const Engine::BodyCameraRig &rig) noexcept { return to_desc(rig.settings()); }

    void body_rig_reset(Engine::BodyCameraRig &rig, std::array<float, 3> position, std::array<float, 4> orientation) noexcept {
        rig.reset(to_vec3(position), to_quat(orientation));
    }

    void body_rig_update(Engine::BodyCameraRig &rig, double dt, std::array<float, 3> target_position, std::array<float, 4> target_orientation,
                         float speed) noexcept {
        rig.update(dt, to_vec3(target_position), to_quat(target_orientation), speed);
    }

    void body_rig_add_translation_impulse(Engine::BodyCameraRig &rig, std::array<float, 3> velocity) noexcept {
        rig.add_translation_impulse(to_vec3(velocity));
    }

    void body_rig_add_rotation_impulse(Engine::BodyCameraRig &rig, std::array<float, 3> angular_velocity) noexcept {
        rig.add_rotation_impulse(to_vec3(angular_velocity));
    }

    BodyRigPose body_rig_pose(const Engine::BodyCameraRig &rig) noexcept {
        BodyRigPose pose;
        store(pose.position, rig.position());
        store(pose.orientation, rig.orientation());
        return pose;
    }

    std::array<float, 16> body_rig_matrix(const Engine::BodyCameraRig &rig) noexcept { return to_array(rig.pose_matrix()); }

    float body_rig_gait_amount(const Engine::BodyCameraRig &rig) noexcept { return rig.gait_amount(); }

} // namespace SFT::CxxApi
