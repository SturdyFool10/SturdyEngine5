#pragma once

#include <array>
#include <memory>

#include <Engine/BodyCameraRig.hpp>

#include <CxxApi/Types/BodyRig.hpp>

/// The body-camera rig (`Engine/BodyCameraRig.hpp`): follow springs, gait and handheld wander between an aim and the camera. Feed it
/// the aim every update, put `body_rig_pose` on the camera. `orientation` quaternions are xyzw.
namespace SFT::CxxApi {

    [[nodiscard]] BodyRigDesc body_rig_desc_defaults() noexcept;
    [[nodiscard]] std::unique_ptr<Engine::BodyCameraRig> body_rig_new(const BodyRigDesc &desc);
    void body_rig_set_settings(Engine::BodyCameraRig &rig, const BodyRigDesc &desc) noexcept;
    [[nodiscard]] BodyRigDesc body_rig_settings(const Engine::BodyCameraRig &rig) noexcept;
    /// Snaps to a pose and clears all motion (a teleport or camera cut).
    void body_rig_reset(Engine::BodyCameraRig &rig, std::array<float, 3> position, std::array<float, 4> orientation) noexcept;
    /// `speed` is the wearer's ground speed in m/s.
    void body_rig_update(Engine::BodyCameraRig &rig, double dt, std::array<float, 3> target_position, std::array<float, 4> target_orientation,
                         float speed) noexcept;
    void body_rig_add_translation_impulse(Engine::BodyCameraRig &rig, std::array<float, 3> velocity) noexcept;
    void body_rig_add_rotation_impulse(Engine::BodyCameraRig &rig, std::array<float, 3> angular_velocity) noexcept;
    /// The pose to render with (follow + gait + wander).
    [[nodiscard]] BodyRigPose body_rig_pose(const Engine::BodyCameraRig &rig) noexcept;
    /// Column-major camera-to-world matrix of the same pose.
    [[nodiscard]] std::array<float, 16> body_rig_matrix(const Engine::BodyCameraRig &rig) noexcept;
    /// 0 = standing, 1 = nominal walk.
    [[nodiscard]] float body_rig_gait_amount(const Engine::BodyCameraRig &rig) noexcept;

} // namespace SFT::CxxApi
