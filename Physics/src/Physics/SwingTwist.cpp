#include <Physics/SwingTwist.hpp>

#include <Animation/Skeleton.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace SFT::Physics {

    namespace {

        // Rotation taking `axis` to +X; the limits are evaluated in that frame so the maths has one shape.
        glm::quat frame_to_x(const glm::vec3 &axis) noexcept {
            return Animation::shortest_arc(axis, glm::vec3(1.0f, 0.0f, 0.0f));
        }

        // Twist about +X in (-pi, pi].
        f32 twist_about_x(const glm::quat &q) noexcept {
            glm::quat twist(q.w, q.x, 0.0f, 0.0f);
            const f32 length = std::sqrt(twist.w * twist.w + twist.x * twist.x);
            if (length < 1e-8f) {
                return 0.0f; // a half-turn swing carries no defined twist
            }
            const f32 angle = 2.0f * std::atan2(twist.x / length, twist.w / length);
            return angle > glm::pi<f32>() ? angle - glm::two_pi<f32>() : (angle <= -glm::pi<f32>() ? angle + glm::two_pi<f32>() : angle);
        }

    } // namespace

    SwingTwist decompose_swing_twist(const glm::quat &q, const glm::vec3 &axis) noexcept {
        const glm::vec3 n = glm::normalize(axis);
        const glm::vec3 projected = n * glm::dot(glm::vec3(q.x, q.y, q.z), n);
        glm::quat twist(q.w, projected.x, projected.y, projected.z);
        const f32 length = std::sqrt(glm::dot(twist, twist));
        SwingTwist out;
        if (length < 1e-8f) {
            // The rotation is a pure half-turn swing: all swing, no twist.
            out.swing = q;
            return out;
        }
        out.twist = twist / length;
        out.swing = q * glm::inverse(out.twist);
        return out;
    }

    f32 twist_angle(const glm::quat &q, const glm::vec3 &axis) noexcept {
        const glm::quat to_x = frame_to_x(glm::normalize(axis));
        return twist_about_x(to_x * q * glm::inverse(to_x));
    }

    bool within_limits(const glm::quat &q, const SwingTwistLimits &limits) noexcept {
        const glm::quat clamped = clamp_to_limits(q, limits);
        // 1 - |dot| ~ angle^2 / 8; the bound is ~1.4 mrad, well above float resolution near 1.
        return 1.0f - std::fabs(glm::dot(clamped, q)) < 1e-6f;
    }

    glm::quat clamp_to_limits(const glm::quat &q, const SwingTwistLimits &limits) noexcept {
        const glm::quat to_x = frame_to_x(glm::normalize(limits.axis));
        const glm::quat local = glm::normalize(to_x * q * glm::inverse(to_x));

        // In the +X frame: local = swing * twist, with swing = (w, 0, y, z).
        const f32 angle = twist_about_x(local);
        const glm::quat twist = glm::angleAxis(angle, glm::vec3(1, 0, 0));
        glm::quat swing = local * glm::inverse(twist);
        if (swing.w < 0.0f) {
            swing = -swing;
        }

        // The swing quaternion's y/z components are sin(half angle) times the axis, so the cone is an ellipse in
        // (y, z) with semi-axes sin(limit / 2).
        const f32 sy = std::sin(std::clamp(limits.swing_y, 0.0f, glm::pi<f32>()) * 0.5f);
        const f32 sz = std::sin(std::clamp(limits.swing_z, 0.0f, glm::pi<f32>()) * 0.5f);
        f32 y = swing.y, z = swing.z;
        const f32 ey = sy > 1e-6f ? y / sy : (std::fabs(y) > 1e-6f ? 1e6f : 0.0f);
        const f32 ez = sz > 1e-6f ? z / sz : (std::fabs(z) > 1e-6f ? 1e6f : 0.0f);
        const f32 e = ey * ey + ez * ez;
        if (e > 1.0f) {
            const f32 scale = 1.0f / std::sqrt(e);
            y = (sy > 1e-6f ? ey * scale * sy : 0.0f);
            z = (sz > 1e-6f ? ez * scale * sz : 0.0f);
        }
        const f32 w = std::sqrt(std::max(0.0f, 1.0f - y * y - z * z));
        swing = glm::quat(w, 0.0f, y, z);

        const f32 clamped_twist = std::clamp(angle, limits.twist_min, limits.twist_max);
        const glm::quat out_local = glm::normalize(swing * glm::angleAxis(clamped_twist, glm::vec3(1, 0, 0)));
        const glm::quat result = glm::normalize(glm::inverse(to_x) * out_local * to_x);
        // Keep the hemisphere of the input so callers interpolating from `q` do not flip.
        return glm::dot(result, q) < 0.0f ? -result : result;
    }

    glm::vec3 pd_drive(const glm::quat &current, const glm::quat &target, const glm::vec3 &angular_velocity,
                       f32 stiffness, f32 damping) noexcept {
        glm::quat error = target * glm::inverse(current);
        if (error.w < 0.0f) {
            error = -error; // take the short way round
        }
        const f32 sin_half = std::sqrt(error.x * error.x + error.y * error.y + error.z * error.z);
        glm::vec3 rotation_vector(0.0f);
        if (sin_half > 1e-7f) {
            const f32 angle = 2.0f * std::atan2(sin_half, error.w);
            rotation_vector = glm::vec3(error.x, error.y, error.z) * (angle / sin_half);
        }
        return stiffness * rotation_vector - damping * angular_velocity;
    }

} // namespace SFT::Physics
