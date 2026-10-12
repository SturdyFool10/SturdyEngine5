#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

/// A body-worn camera's motion, decoupled from where the wearer is aiming: the chest / helmet / shoulder rig of a bodycam or
/// found-footage view. The *aim* (position and orientation, e.g. from a player controller) is the input; the camera that
/// follows it is what you render with:
///
///  * **Follow springs.** Position and orientation chase the aim through critically-damped springs, so the camera lags a quick
///    turn and settles, instead of being bolted to the crosshair. Stable at any frame time (sub-stepped).
///  * **Gait.** Walking adds a vertical bob at twice the stride rate, a lateral sway and a roll at the stride rate, all scaled by
///    speed (and eased in and out, so starting and stopping is smooth); the stride rate follows speed / stride length.
///  * **Handheld noise.** A slow, smooth, deterministic wander in rotation and position: the camera is never perfectly still.
///  * **Impulses.** `add_translation_impulse` / `add_rotation_impulse` kick the springs (a stumble, recoil, an explosion); the
///    springs absorb them.
///
/// Pure CPU, deterministic (same inputs, same pose), no engine objects: unit-testable and usable from any thread.
///
/// ```cpp
/// BodyCameraRig rig;
/// rig.reset(player.position, player.aim);
/// // every update:
/// rig.update(dt, player.position + eye_offset, player.aim, player.speed);
/// camera.set_position(rig.position());
/// camera.set_orientation(rig.orientation());
/// ```
namespace SFT::Engine {

    struct BodyCameraRigSettings {
        /// How fast the camera catches up with the aim, in hertz (natural frequency of the springs): higher = tighter.
        f32 position_follow_hz = 9.0f;
        f32 rotation_follow_hz = 5.0f;
        /// 1 = critically damped (no overshoot); below 1 the camera overshoots slightly and wobbles.
        f32 damping_ratio = 1.0f;

        /// Stride length in metres: the stride rate is speed / stride_length.
        f32 stride_length = 1.5f;
        /// Speed (m/s) at which the gait is at its nominal amplitude; it scales linearly up to 1.5x beyond that.
        f32 gait_reference_speed = 1.6f;
        f32 bob_amplitude = 0.035f;  // metres, vertical, twice per stride
        f32 sway_amplitude = 0.02f;  // metres, lateral, once per stride
        f32 roll_amplitude_degrees = 0.7f;
        f32 pitch_amplitude_degrees = 0.45f;
        /// How quickly the gait fades in and out when speed changes, per second.
        f32 gait_blend_rate = 6.0f;

        /// Handheld wander: rotation (degrees, about each axis) and position (metres), and how fast it drifts (hertz).
        f32 handheld_rotation_degrees = 0.18f;
        f32 handheld_position = 0.004f;
        f32 handheld_frequency_hz = 0.8f;
        /// Seeds the wander, so two rigs do not move in lockstep.
        u32 seed = 1;
    };

    class BodyCameraRig {
      public:
        explicit BodyCameraRig(const BodyCameraRigSettings &settings = {}) noexcept : settings_(settings) {}

        void set_settings(const BodyCameraRigSettings &settings) noexcept { settings_ = settings; }
        [[nodiscard]] const BodyCameraRigSettings &settings() const noexcept { return settings_; }

        /// Snaps the rig to a pose and clears all motion (a teleport or camera cut).
        void reset(const glm::vec3 &position, const glm::quat &orientation) noexcept;

        /// Advances by `dt` seconds towards the aim. `speed` is the wearer's ground speed in m/s (drives the gait). Non-finite
        /// inputs are ignored for that call.
        void update(f64 dt, const glm::vec3 &target_position, const glm::quat &target_orientation, f32 speed) noexcept;

        /// Kicks the position spring (metres per second) / the rotation spring (radians per second, about the world axes).
        void add_translation_impulse(const glm::vec3 &velocity) noexcept;
        void add_rotation_impulse(const glm::vec3 &angular_velocity) noexcept;

        /// The camera pose to render with.
        [[nodiscard]] glm::vec3 position() const noexcept;
        [[nodiscard]] glm::quat orientation() const noexcept;
        /// The same pose as a camera-to-world matrix.
        [[nodiscard]] glm::mat4 pose_matrix() const noexcept;

        /// The springs' state without gait and wander: where the camera would be if it only followed.
        [[nodiscard]] const glm::vec3 &followed_position() const noexcept { return position_; }
        [[nodiscard]] const glm::quat &followed_orientation() const noexcept { return orientation_; }
        /// How much of the gait is currently applied (0 = standing, 1 = nominal walk).
        [[nodiscard]] f32 gait_amount() const noexcept { return gait_amount_; }

      private:
        BodyCameraRigSettings settings_;
        glm::vec3 position_{0.0f};
        glm::vec3 velocity_{0.0f};
        glm::quat orientation_{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3 angular_velocity_{0.0f};
        glm::quat target_orientation_{1.0f, 0.0f, 0.0f, 0.0f};
        f32 gait_amount_ = 0.0f;
        f64 gait_phase_ = 0.0;
        f64 time_ = 0.0;
    };

} // namespace SFT::Engine
