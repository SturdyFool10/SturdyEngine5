#pragma once

#include <Foundation/Foundation.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace SFT::Physics {

    /// Rotation split around a twist axis: `rotation == swing * twist`.
    struct SwingTwist {
        glm::quat swing{1.0f, 0.0f, 0.0f, 0.0f};
        glm::quat twist{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /// Limits of a ball-and-socket joint (shoulder, hip, spine) in its local frame, whose twist axis is `axis`.
    struct SwingTwistLimits {
        /// Unit twist axis in the joint's frame (the bone direction).
        glm::vec3 axis{1.0f, 0.0f, 0.0f};
        /// Half-angles of the elliptical swing cone, in radians. Zero locks that direction.
        f32 swing_y = 0.8f;
        f32 swing_z = 0.8f;
        /// Allowed twist range, radians, about `axis`.
        f32 twist_min = -0.5f;
        f32 twist_max = 0.5f;
    };

    /// Splits `q` into swing and twist about the unit vector `axis`.
    [[nodiscard]] SwingTwist decompose_swing_twist(const glm::quat &q, const glm::vec3 &axis) noexcept;

    /// Signed twist angle in (-pi, pi] about `axis`.
    [[nodiscard]] f32 twist_angle(const glm::quat &q, const glm::vec3 &axis) noexcept;

    /// True when `q` is inside the limits (with a small tolerance).
    [[nodiscard]] bool within_limits(const glm::quat &q, const SwingTwistLimits &limits) noexcept;

    /// The closest rotation to `q` that satisfies `limits`: twist clamped to its range, swing pulled back inside the
    /// elliptical cone. `q` is returned unchanged when already legal.
    [[nodiscard]] glm::quat clamp_to_limits(const glm::quat &q, const SwingTwistLimits &limits) noexcept;

    /// Active-ragdoll drive: the angular velocity (rad/s, in the parent's frame) a motor applies to pull a joint from
    /// `current` toward `target`, as a critically-tunable PD controller. `angular_velocity` is the joint's present
    /// relative velocity.
    [[nodiscard]] glm::vec3 pd_drive(const glm::quat &current, const glm::quat &target, const glm::vec3 &angular_velocity,
                                     f32 stiffness, f32 damping) noexcept;

} // namespace SFT::Physics
